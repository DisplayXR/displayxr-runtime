// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-065 / ADR-066 C1: apps see a display driver's table as two slots,
 *         [0] = 2D, [1] = 3D; the runtime keeps a longer declared list as the
 *         sizing envelope and remaps slot 1 live.
 *
 * Pins the pure rule (u_rendering_modes_trim) on the shapes that exist or have
 * existed: a 1-mode device, the shipping 2-mode shape, a vendor table with an
 * optional third mode, the pre-ADR-065 six-mode sim-display table started in
 * each of its modes, and the two refusals. Then the device-level apply
 * (u_device_enforce_two_rendering_modes) on a fake head: the recomputed max
 * view count, the worst-case atlas recomputed from the trimmed table, and the
 * OUTPUT_MODE translation for a driver whose kept 3D mode is not its index 1.
 */

#include "catch_amalgamated.hpp"

#include "util/u_rendering_modes.h"
#include "util/u_tiling.h"
#include "xrt/xrt_display_status.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

xrt_rendering_mode
mk(uint32_t views, float sx, float sy, bool hw3d, uint32_t cols, uint32_t rows, const char *name)
{
	xrt_rendering_mode m{};
	m.view_count = views;
	m.view_scale_x = sx;
	m.view_scale_y = sy;
	m.hardware_display_3d = hw3d;
	m.tile_columns = cols;
	m.tile_rows = rows;
	std::snprintf(m.mode_name, sizeof(m.mode_name), "%s", name);
	return m;
}

//! The pre-ADR-065 sim-display table, verbatim (6 modes, mode_index = position).
std::vector<xrt_rendering_mode>
old_sim_table()
{
	std::vector<xrt_rendering_mode> t = {
	    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),          //
	    mk(2, 0.5f, 0.5f, true, 2, 1, "Anaglyph"),     //
	    mk(2, 0.5f, 0.5f, true, 1, 2, "Cropped SBS"),  //
	    mk(2, 0.5f, 1.0f, true, 2, 1, "Squeezed SBS"), //
	    mk(4, 0.5f, 0.5f, true, 2, 2, "Quad"),         //
	    mk(2, 0.5f, 0.5f, true, 2, 1, "Interlaced"),   //
	};
	for (uint32_t i = 0; i < t.size(); i++) {
		t[i].mode_index = i;
	}
	return t;
}

struct table
{
	xrt_rendering_mode modes[XRT_MAX_RENDERING_MODES]{};
	uint32_t count = 0;
	uint32_t active = 0;

	table(const std::vector<xrt_rendering_mode> &v, uint32_t act)
	{
		count = (uint32_t)v.size();
		for (uint32_t i = 0; i < count; i++) {
			modes[i] = v[i];
		}
		active = act;
	}

	u_rendering_modes_trim_info
	trim(u_rendering_modes_trim_result expect)
	{
		u_rendering_modes_trim_info info{};
		CHECK(u_rendering_modes_trim(modes, &count, &active, U_RENDERING_MODES_NO_PREFERENCE, &info) == expect);
		CHECK(info.result == expect);
		return info;
	}
};

} // namespace

TEST_CASE("a one-mode table is left alone", "[u_rendering_modes]")
{
	table t({mk(1, 1.0f, 1.0f, false, 1, 1, "2D")}, 0);
	const auto info = t.trim(U_RENDERING_MODES_TRIM_UNCHANGED);
	CHECK(t.count == 1);
	CHECK(t.active == 0);
	CHECK(info.envelope_count == 1);
	CHECK_FALSE(info.renumbered);
}

TEST_CASE("a two-mode table is left alone", "[u_rendering_modes]")
{
	SECTION("active 2D")
	{
		table t({mk(1, 1.0f, 1.0f, false, 1, 1, "2D"), mk(2, 0.5f, 1.0f, true, 2, 1, "3D")}, 0);
		t.modes[1].mode_index = 1;
		const auto info = t.trim(U_RENDERING_MODES_TRIM_UNCHANGED);
		CHECK(t.count == 2);
		CHECK(t.active == 0);
		CHECK(info.to_driver[1] == 1);
		CHECK_FALSE(info.renumbered);
	}
	SECTION("active 3D")
	{
		table t({mk(1, 1.0f, 1.0f, false, 1, 1, "2D"), mk(4, 0.5f, 0.5f, true, 2, 2, "Quad")}, 1);
		t.modes[1].mode_index = 1;
		t.trim(U_RENDERING_MODES_TRIM_UNCHANGED);
		CHECK(t.count == 2);
		CHECK(t.active == 1);
		CHECK(std::strcmp(t.modes[1].mode_name, "Quad") == 0);
	}
	SECTION("a vendor-numbered mode_index is renumbered to the array position")
	{
		table t({mk(1, 1.0f, 1.0f, false, 1, 1, "2D"), mk(2, 0.5f, 1.0f, true, 2, 1, "3D")}, 1);
		t.modes[0].mode_index = 10;
		t.modes[1].mode_index = 11;
		const auto info = t.trim(U_RENDERING_MODES_TRIM_UNCHANGED);
		CHECK(info.renumbered);
		CHECK(t.modes[0].mode_index == 0);
		CHECK(t.modes[1].mode_index == 1);
		CHECK(t.active == 1);
	}
}

TEST_CASE("a vendor table with an optional third mode keeps 2D and its index-1 3D mode", "[u_rendering_modes]")
{
	// The shape: 2D, the 3D mode, and a variant of it with the same atlas.
	const std::vector<xrt_rendering_mode> v = {
	    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	    mk(2, 0.5f, 1.0f, true, 2, 1, "3D"),
	    mk(2, 0.5f, 1.0f, true, 2, 1, "3D (variant)"),
	};

	SECTION("active = 1 (the device's default)")
	{
		table t(v, 1);
		const auto info = t.trim(U_RENDERING_MODES_TRIM_TRIMMED);
		CHECK(t.count == 2);
		CHECK(t.active == 1);
		CHECK(info.to_driver[1] == 1); // identity: no translation needed
		CHECK(info.envelope_count == 3); // ADR-066 C1: nothing dropped
		CHECK(std::strcmp(t.modes[1].mode_name, "3D") == 0);
		CHECK(std::strcmp(t.modes[2].mode_name, "3D (variant)") == 0);
	}
	SECTION("active = 0")
	{
		table t(v, 0);
		const auto info = t.trim(U_RENDERING_MODES_TRIM_TRIMMED);
		CHECK(t.active == 0);
		CHECK(info.to_driver[1] == 1);
	}
	SECTION("active = 2: the variant the device started in is the one kept")
	{
		table t(v, 2);
		const auto info = t.trim(U_RENDERING_MODES_TRIM_TRIMMED);
		CHECK(t.active == 1);
		CHECK(info.to_driver[1] == 2);
		CHECK(info.from_driver[2] == 1);
		CHECK(info.from_driver[1] == 1); // dropped, but a 3D mode
		CHECK(std::strcmp(t.modes[1].mode_name, "3D (variant)") == 0);
		// ADR-066 C1: the driver's entry 1 is kept, swapped to where the variant was.
		CHECK(std::strcmp(t.modes[2].mode_name, "3D") == 0);
		CHECK(t.modes[1].mode_index == 1);
	}
}

TEST_CASE("the old six-mode sim-display table trims to 2D + the mode it started in", "[u_rendering_modes]")
{
	struct row
	{
		uint32_t active;
		uint32_t expect_kept;
		uint32_t expect_active;
		const char *kept_name;
		uint32_t kept_views;
	};
	const row rows[] = {
	    {0, 1, 0, "Anaglyph", 2},     // started in 2D: the first 3D mode is kept
	    {1, 1, 1, "Anaglyph", 2},     //
	    {3, 3, 1, "Squeezed SBS", 2}, // active = 3
	    {4, 4, 1, "Quad", 4},         //
	    {5, 5, 1, "Interlaced", 2},   //
	};
	for (const row &r : rows) {
		INFO("active at creation = " << r.active);
		table t(old_sim_table(), r.active);
		const auto info = t.trim(U_RENDERING_MODES_TRIM_TRIMMED);
		CHECK(t.count == 2);
		CHECK(t.active == r.expect_active);
		CHECK(info.original_count == 6);
		CHECK(info.to_driver[0] == 0);
		CHECK(info.to_driver[1] == r.expect_kept);
		CHECK(std::strcmp(t.modes[0].mode_name, "2D") == 0);
		CHECK(std::strcmp(t.modes[1].mode_name, r.kept_name) == 0);
		CHECK(t.modes[0].mode_index == 0);
		CHECK(t.modes[1].mode_index == 1);
		CHECK(u_rendering_modes_max_view_count(t.modes, t.count) == r.kept_views);
		// ADR-066 C1: nothing is dropped - the full list stays as the envelope,
		// the kept entry swapped into slot 1.
		CHECK(info.envelope_count == 6);
		CHECK(std::strcmp(t.modes[r.expect_kept].mode_name, old_sim_table()[1].mode_name) == 0);
		for (uint32_t i = 0; i < 6; i++) {
			CHECK(t.modes[i].mode_index == i);
		}
		// Each original index maps to a run-time index: 2D -> 0, any 3D -> 1.
		CHECK(info.from_driver[0] == 0);
		for (uint32_t i = 1; i < 6; i++) {
			CHECK(info.from_driver[i] == 1);
		}
	}
}

TEST_CASE("a table whose mode 0 is not the flat mode is refused, untouched", "[u_rendering_modes]")
{
	// Decision (ADR-065): the runtime does not guess which entry is the flat
	// mode. It keeps the driver's table and WARNs that the contract is not met.
	table t(
	    {
	        mk(2, 0.5f, 1.0f, true, 2, 1, "3D first"),
	        mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	        mk(4, 0.5f, 0.5f, true, 2, 2, "Quad"),
	    },
	    2);
	const auto info = t.trim(U_RENDERING_MODES_TRIM_REFUSED);
	CHECK(t.count == 3);
	CHECK(t.active == 2);
	CHECK(std::strcmp(t.modes[0].mode_name, "3D first") == 0);
	CHECK(std::strcmp(t.modes[2].mode_name, "Quad") == 0);
	for (uint32_t i = 0; i < 3; i++) {
		CHECK(info.from_driver[i] == i);
		CHECK(t.modes[i].mode_index == i);
	}
}

TEST_CASE("a long table with no 3D mode is refused too", "[u_rendering_modes]")
{
	table t(
	    {
	        mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	        mk(1, 0.5f, 0.5f, false, 1, 1, "2D half"),
	        mk(1, 0.25f, 0.25f, false, 1, 1, "2D quarter"),
	    },
	    1);
	t.trim(U_RENDERING_MODES_TRIM_REFUSED);
	CHECK(t.count == 3);
	CHECK(t.active == 1);
}

TEST_CASE("the max view count and the worst-case atlas come from the full envelope", "[u_rendering_modes]")
{
	// ADR-066 C1.5: sizing goes back to the full declared list. A 3x2 quilt at
	// half scale is 1.5x the panel's width: it sizes the atlas and the view
	// count whichever entry slot 1 maps to, so a later switch to it needs no
	// reallocation.
	const std::vector<xrt_rendering_mode> v = {
	    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	    mk(2, 0.5f, 1.0f, true, 2, 1, "Stereo"),
	    mk(6, 0.5f, 0.5f, true, 3, 2, "Quilt 3x2"),
	};
	const uint32_t W = 1920, H = 1080;
	for (uint32_t start : {1u, 2u}) {
		INFO("started in " << start);
		table t(v, start);
		const auto info = t.trim(U_RENDERING_MODES_TRIM_TRIMMED);
		REQUIRE(info.envelope_count == 3);
		for (uint32_t i = 0; i < info.envelope_count; i++) {
			u_tiling_compute_mode(&t.modes[i], W, H);
		}
		uint32_t aw = 0, ah = 0;
		u_tiling_compute_system_atlas(t.modes, info.envelope_count, &aw, &ah);
		CHECK(aw == 2880);
		CHECK(ah == 1080);
		CHECK(u_rendering_modes_max_view_count(t.modes, info.envelope_count) == 6);
		// The two presented slots alone would under-size it.
		CHECK(u_rendering_modes_max_view_count(t.modes, t.count) == (start == 1 ? 2u : 6u));
	}
}

TEST_CASE("degenerate trim inputs are harmless", "[u_rendering_modes]")
{
	uint32_t count = 3, active = 0;
	CHECK(u_rendering_modes_trim(nullptr, &count, &active, U_RENDERING_MODES_NO_PREFERENCE, nullptr) ==
	      U_RENDERING_MODES_TRIM_UNCHANGED);
	CHECK(count == 3);
	xrt_rendering_mode m[XRT_MAX_RENDERING_MODES]{};
	CHECK(u_rendering_modes_trim(m, nullptr, &active, U_RENDERING_MODES_NO_PREFERENCE, nullptr) ==
	      U_RENDERING_MODES_TRIM_UNCHANGED);
	// A NULL active is allowed: the trim picks the first 3D mode.
	table t(old_sim_table(), 0);
	CHECK(u_rendering_modes_trim(t.modes, &t.count, nullptr, U_RENDERING_MODES_NO_PREFERENCE, nullptr) ==
	      U_RENDERING_MODES_TRIM_TRIMMED);
	CHECK(t.count == 2);
	CHECK(std::strcmp(t.modes[1].mode_name, "Anaglyph") == 0);
	// An out-of-range active becomes 2D.
	table o(old_sim_table(), 7);
	o.trim(U_RENDERING_MODES_TRIM_TRIMMED);
	CHECK(o.active == 0);
}


/*
 *
 * The device-level apply.
 *
 */

namespace {

//! A head device whose set_property tracks the active mode in ITS numbering,
//! like the pre-ADR-065 sim-display did (minus the bounds check on its count).
struct fake_head
{
	xrt_device xdev{};
	xrt_hmd_parts hmd{};
	std::vector<int32_t> set_calls;
	int32_t driver_output_mode = 0;
	bool destroyed = false;

	static fake_head *
	of(xrt_device *x)
	{
		return reinterpret_cast<fake_head *>(x);
	}

	static xrt_result_t
	set_prop(xrt_device *x, xrt_device_property_id p, int32_t v)
	{
		fake_head *h = of(x);
		if (p != XRT_DEVICE_PROPERTY_OUTPUT_MODE) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		h->set_calls.push_back(v);
		h->driver_output_mode = v;
		h->hmd.active_rendering_mode_index = (uint32_t)v;
		return XRT_SUCCESS;
	}

	static xrt_result_t
	get_prop(xrt_device *x, xrt_device_property_id p, int32_t *out)
	{
		if (p != XRT_DEVICE_PROPERTY_OUTPUT_MODE) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		*out = of(x)->driver_output_mode;
		return XRT_SUCCESS;
	}

	static void
	destroy(xrt_device *x)
	{
		of(x)->destroyed = true;
	}

	fake_head(const std::vector<xrt_rendering_mode> &v, uint32_t active)
	{
		// xdev is the first member: of() casts back to the fake.
		xdev.rendering_mode_count = (uint32_t)v.size();
		uint32_t max_views = 1;
		for (uint32_t i = 0; i < v.size(); i++) {
			xdev.rendering_modes[i] = v[i];
			max_views = v[i].view_count > max_views ? v[i].view_count : max_views;
		}
		hmd.view_count = max_views;
		hmd.active_rendering_mode_index = active;
		driver_output_mode = (int32_t)active;
		xdev.hmd = &hmd;
		xdev.set_property = set_prop;
		xdev.get_property = get_prop;
		xdev.destroy = destroy;
		std::snprintf(xdev.str, sizeof(xdev.str), "fake head");
	}
};

} // namespace

TEST_CASE("enforce: a two-mode device is untouched, functions and all", "[u_rendering_modes][device]")
{
	fake_head h({mk(1, 1.0f, 1.0f, false, 1, 1, "2D"), mk(2, 0.5f, 1.0f, true, 2, 1, "3D")}, 1);
	h.xdev.rendering_modes[1].mode_index = 1;
	CHECK(u_device_enforce_two_rendering_modes(&h.xdev, "test") == U_RENDERING_MODES_TRIM_UNCHANGED);
	CHECK(h.xdev.set_property == fake_head::set_prop);
	CHECK(h.xdev.destroy == fake_head::destroy);
	CHECK(h.hmd.view_count == 2);
}

TEST_CASE("enforce: a kept index 1 needs no translation", "[u_rendering_modes][device]")
{
	fake_head h(
	    {
	        mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	        mk(2, 0.5f, 1.0f, true, 2, 1, "3D"),
	        mk(2, 0.5f, 1.0f, true, 2, 1, "3D (variant)"),
	    },
	    1);
	CHECK(u_device_enforce_two_rendering_modes(&h.xdev, "test") == U_RENDERING_MODES_TRIM_TRIMMED);
	CHECK(h.xdev.rendering_mode_count == 2);
	CHECK(u_device_rendering_modes_envelope_count(&h.xdev) == 3);
	// ADR-066 C1: wrapped anyway, so a later remap needs no new wrapper; the
	// translation is identity today.
	CHECK(h.xdev.set_property != fake_head::set_prop);
	CHECK(h.hmd.active_rendering_mode_index == 1);
	CHECK(xrt_device_set_property(&h.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 1) == XRT_SUCCESS);
	CHECK(h.set_calls.back() == 1);
	h.xdev.destroy(&h.xdev);
}

TEST_CASE("enforce: the old sim-display started in Quad", "[u_rendering_modes][device]")
{
	fake_head h(old_sim_table(), 4);
	REQUIRE(h.hmd.view_count == 4);
	CHECK(u_device_enforce_two_rendering_modes(&h.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
	CHECK(h.xdev.rendering_mode_count == 2);
	CHECK(h.hmd.active_rendering_mode_index == 1);
	CHECK(h.hmd.view_count == 4); // Quad kept: still 4

	SECTION("OUTPUT_MODE crosses in the driver's numbering, both ways")
	{
		REQUIRE(h.xdev.set_property != fake_head::set_prop); // wrapped

		CHECK(xrt_device_set_property(&h.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 0) == XRT_SUCCESS);
		CHECK(h.set_calls.back() == 0);
		CHECK(h.hmd.active_rendering_mode_index == 0);
		int32_t got = -1;
		CHECK(xrt_device_get_property(&h.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, &got) == XRT_SUCCESS);
		CHECK(got == 0);

		CHECK(xrt_device_set_property(&h.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 1) == XRT_SUCCESS);
		CHECK(h.set_calls.back() == 4); // the driver's own Quad index
		// The driver wrote 4 into the active index; the wrapper repoints it.
		CHECK(h.hmd.active_rendering_mode_index == 1);
		CHECK(xrt_device_get_property(&h.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, &got) == XRT_SUCCESS);
		CHECK(got == 1);

		// Not a run-time mode: refused before it reaches the driver.
		const size_t calls = h.set_calls.size();
		CHECK(xrt_device_set_property(&h.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 2) ==
		      XRT_ERROR_NOT_IMPLEMENTED);
		CHECK(h.set_calls.size() == calls);
	}

	SECTION("a second enforce is a no-op")
	{
		CHECK(u_device_enforce_two_rendering_modes(&h.xdev, "sim-display") == U_RENDERING_MODES_TRIM_UNCHANGED);
		CHECK(xrt_device_set_property(&h.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 1) == XRT_SUCCESS);
		CHECK(h.set_calls.back() == 4);
	}

	SECTION("destroy reaches the driver and frees the translation slot")
	{
		xrt_device *x = &h.xdev;
		x->destroy(x);
		CHECK(h.destroyed);
	}

	// Leave no wrapper behind for the next test that lands at this address.
	if (!h.destroyed) {
		h.xdev.destroy(&h.xdev);
	}
}

TEST_CASE("enforce: started in 2D, the device max view count stays the envelope's", "[u_rendering_modes][device]")
{
	fake_head h(old_sim_table(), 0);
	CHECK(u_device_enforce_two_rendering_modes(&h.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
	CHECK(h.hmd.active_rendering_mode_index == 0);
	CHECK(h.hmd.view_count == 4); // ADR-066 C1.5: Quad is still in the envelope
	CHECK(u_device_rendering_modes_envelope_count(&h.xdev) == 6);
	h.xdev.destroy(&h.xdev);
}

TEST_CASE("enforce: a refused table keeps its count and functions", "[u_rendering_modes][device]")
{
	fake_head h(
	    {
	        mk(2, 0.5f, 1.0f, true, 2, 1, "3D first"),
	        mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	        mk(4, 0.5f, 0.5f, true, 2, 2, "Quad"),
	    },
	    0);
	CHECK(u_device_enforce_two_rendering_modes(&h.xdev, "odd") == U_RENDERING_MODES_TRIM_REFUSED);
	CHECK(h.xdev.rendering_mode_count == 3);
	CHECK(h.hmd.view_count == 4);
	CHECK(h.xdev.set_property == fake_head::set_prop);
}


/*
 *
 * A REALISTIC pre-ADR-065 driver: range-checks the index it is handed against
 * its own rendering_mode_count and reads rendering_modes[index] - what the
 * released six-mode sim-display does. The fake above omits both; this one is
 * the case the wrapper exists for.
 *
 */

namespace {

struct strict_driver
{
	xrt_device xdev{};
	xrt_hmd_parts hmd{};
	int32_t output = 0;               //!< the driver's own output mode (its numbering)
	bool pinned = false;              //!< FORCE_MODE-style pin
	std::vector<int32_t> accepted;    //!< indices it accepted
	std::vector<int32_t> refused;     //!< indices it refused
	std::vector<uint32_t> seen_count; //!< rendering_mode_count it saw on each call
	std::vector<uint32_t> seen_views; //!< rendering_modes[value].view_count it read
	bool destroyed = false;

	static strict_driver *
	of(xrt_device *x)
	{
		return reinterpret_cast<strict_driver *>(x); // xdev is the first member
	}

	static xrt_result_t
	set_prop(xrt_device *x, xrt_device_property_id p, int32_t v)
	{
		strict_driver *d = of(x);
		if (p != XRT_DEVICE_PROPERTY_OUTPUT_MODE) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		d->seen_count.push_back(x->rendering_mode_count);
		if (d->pinned) {
			return XRT_SUCCESS; // swallowed
		}
		if (v < 0 || (uint32_t)v >= x->rendering_mode_count) {
			d->refused.push_back(v);
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		d->accepted.push_back(v);
		d->seen_views.push_back(x->rendering_modes[v].view_count);
		x->hmd->active_rendering_mode_index = (uint32_t)v;
		d->output = v;
		return XRT_SUCCESS;
	}

	static xrt_result_t
	get_prop(xrt_device *x, xrt_device_property_id p, int32_t *out)
	{
		strict_driver *d = of(x);
		if (p == XRT_DEVICE_PROPERTY_OUTPUT_MODE) {
			*out = d->output;
			return XRT_SUCCESS;
		}
		if (p == XRT_DEVICE_PROPERTY_OUTPUT_MODE_PINNED) {
			*out = d->pinned ? 1 : 0;
			return XRT_SUCCESS;
		}
		return XRT_ERROR_NOT_IMPLEMENTED;
	}

	static void
	destroy(xrt_device *x)
	{
		of(x)->destroyed = true;
	}

	strict_driver(const std::vector<xrt_rendering_mode> &v, uint32_t active, bool pin = false)
	{
		xdev.rendering_mode_count = (uint32_t)v.size();
		uint32_t max_views = 1;
		for (uint32_t i = 0; i < v.size(); i++) {
			xdev.rendering_modes[i] = v[i];
			max_views = v[i].view_count > max_views ? v[i].view_count : max_views;
		}
		hmd.view_count = max_views;
		hmd.active_rendering_mode_index = active;
		output = (int32_t)active;
		pinned = pin;
		xdev.hmd = &hmd;
		xdev.set_property = set_prop;
		xdev.get_property = get_prop;
		xdev.destroy = destroy;
		std::snprintf(xdev.str, sizeof(xdev.str), "strict driver");
	}

	~strict_driver()
	{
		if (!destroyed && xdev.destroy != nullptr) {
			xdev.destroy(&xdev); // frees the wrapper slot for the next test
		}
	}

	//! What every runtime writer does: write the run-time index, then tell the device.
	void
	runtime_select(uint32_t runtime_idx)
	{
		hmd.active_rendering_mode_index = runtime_idx;
		xrt_device_set_property(&xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, (int32_t)runtime_idx);
	}

	int32_t
	runtime_output()
	{
		int32_t v = -1;
		REQUIRE(xrt_device_get_property(&xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, &v) == XRT_SUCCESS);
		return v;
	}
};

void
rewrite_full_table(strict_driver &d, const std::vector<xrt_rendering_mode> &full)
{
	for (uint32_t i = 0; i < full.size(); i++) {
		d.xdev.rendering_modes[i] = full[i];
	}
	d.xdev.rendering_mode_count = (uint32_t)full.size();
}

} // namespace

TEST_CASE("F1: a range-checking pre-ADR-065 driver really switches through the wrapper", "[u_rendering_modes][device]")
{
	strict_driver d(old_sim_table(), /* started in Quad */ 4);
	REQUIRE(u_device_enforce_two_rendering_modes(&d.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
	REQUIRE(d.xdev.rendering_mode_count == 2);
	REQUIRE(d.hmd.active_rendering_mode_index == 1);
	REQUIRE(d.xdev.set_property != strict_driver::set_prop); // wrapped
	CHECK(u_device_rendering_modes_kept_driver_index(&d.xdev) == 4);

	SECTION("V to 2D and back")
	{
		d.runtime_select(0);
		CHECK(d.output == 0);
		CHECK(d.hmd.active_rendering_mode_index == 0);
		CHECK(d.runtime_output() == 0);

		d.runtime_select(1);
		CHECK(d.refused.empty()); // the defect: it used to refuse its own index 4
		CHECK(d.output == 4);
		CHECK(d.seen_views.back() == 4);               // it read ITS Quad entry
		CHECK(d.seen_count.back() == 6);               // it saw its own table during the call
		CHECK(d.xdev.rendering_mode_count == 2);       // ...and the runtime sees two after it
		CHECK(d.hmd.active_rendering_mode_index == 1); // repointed from the driver index 4
		CHECK(d.runtime_output() == 1);
		CHECK(std::strcmp(d.xdev.rendering_modes[1].mode_name, "Quad") == 0);
	}

	SECTION("session end -> begin restore")
	{
		// xrEndSession: OUTPUT_MODE 0, then the runtime's active 0.
		xrt_device_set_property(&d.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 0);
		d.hmd.active_rendering_mode_index = 0;
		CHECK(d.output == 0);
		// xrBeginSession restores the ended mode (#1041): 1.
		d.runtime_select(1);
		CHECK(d.output == 4);
		CHECK(d.refused.empty());
		CHECK(d.hmd.active_rendering_mode_index == 1);
		CHECK(d.xdev.rendering_mode_count == 2);
	}

	SECTION("run-time indices past 1 never reach the driver")
	{
		CHECK(xrt_device_set_property(&d.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 2) ==
		      XRT_ERROR_NOT_IMPLEMENTED);
		CHECK(d.seen_count.empty());
	}

	SECTION("PINNED round-trips through the wrapper untouched")
	{
		int32_t pinned = -1;
		CHECK(xrt_device_get_property(&d.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE_PINNED, &pinned) == XRT_SUCCESS);
		CHECK(pinned == 0);
	}
}

TEST_CASE("F1: a PINNED pre-ADR-065 driver holds its mode through the wrapper", "[u_rendering_modes][device]")
{
	strict_driver d(old_sim_table(), 4, /* pinned */ true);
	REQUIRE(u_device_enforce_two_rendering_modes(&d.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
	int32_t pinned = 0;
	CHECK(xrt_device_get_property(&d.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE_PINNED, &pinned) == XRT_SUCCESS);
	CHECK(pinned == 1);
	CHECK(xrt_device_set_property(&d.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 0) == XRT_SUCCESS); // swallowed
	CHECK(d.output == 4);
	CHECK(d.runtime_output() == 1); // the driver index 4, in run-time numbering
	CHECK(d.xdev.rendering_mode_count == 2);
}

TEST_CASE("F3: trim, the plug-in rewrites its full table, re-trim keeps the same kind", "[u_rendering_modes][device]")
{
	strict_driver d(old_sim_table(), 4);
	REQUIRE(u_device_enforce_two_rendering_modes(&d.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
	auto *const wrapped_set = d.xdev.set_property;
	REQUIRE(wrapped_set != strict_driver::set_prop);

	SECTION("a refresh on the trimmed table is a no-op")
	{
		CHECK(u_device_reenforce_two_rendering_modes(&d.xdev, "sim-display") ==
		      U_RENDERING_MODES_TRIM_UNCHANGED);
		CHECK(d.xdev.rendering_mode_count == 2);
		CHECK(d.xdev.set_property == wrapped_set);
	}

	SECTION("rewritten while in 3D: the same driver index, active still 1, wrappers not reinstalled")
	{
		rewrite_full_table(d, old_sim_table());
		// The runtime's active index is 1 (run-time numbering) - which in the
		// driver numbering would be Anaglyph. The re-trim must not read it so.
		REQUIRE(d.hmd.active_rendering_mode_index == 1);

		CHECK(u_device_reenforce_two_rendering_modes(&d.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
		CHECK(d.xdev.rendering_mode_count == 2);
		CHECK(std::strcmp(d.xdev.rendering_modes[1].mode_name, "Quad") == 0);
		CHECK(d.hmd.active_rendering_mode_index == 1);
		CHECK(d.hmd.view_count == 4);
		CHECK(d.xdev.set_property == wrapped_set);
		CHECK(u_device_rendering_modes_kept_driver_index(&d.xdev) == 4);

		d.runtime_select(1);
		CHECK(d.output == 4);
		CHECK(d.refused.empty());
	}

	SECTION("rewritten while in 2D: active stays 0")
	{
		d.runtime_select(0);
		rewrite_full_table(d, old_sim_table());
		CHECK(u_device_reenforce_two_rendering_modes(&d.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
		CHECK(d.hmd.active_rendering_mode_index == 0);
		CHECK(std::strcmp(d.xdev.rendering_modes[1].mode_name, "Quad") == 0);
	}

	SECTION("the kept index is no longer 3D: the first 3D mode, and the map follows")
	{
		auto full = old_sim_table();
		full[4] = mk(1, 1.0f, 1.0f, false, 1, 1, "2D again");
		rewrite_full_table(d, full);
		CHECK(u_device_reenforce_two_rendering_modes(&d.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
		CHECK(std::strcmp(d.xdev.rendering_modes[1].mode_name, "Anaglyph") == 0);
		CHECK(u_device_rendering_modes_kept_driver_index(&d.xdev) == 1);
		CHECK(d.xdev.set_property == wrapped_set); // still never re-wrapped
		d.runtime_select(1);
		CHECK(d.output == 1);
	}
}

TEST_CASE("F4: the V / 1 key target is refused when the session cannot fill mode 1", "[u_rendering_modes]")
{
	const std::vector<xrt_rendering_mode> quad = {mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	                                              mk(4, 0.5f, 0.5f, true, 2, 2, "Quad")};
	const std::vector<xrt_rendering_mode> stereo = {mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	                                                mk(2, 0.5f, 0.5f, true, 2, 1, "Anaglyph")};
	// 0 = no limit (orchestrator, pinned, service, floor off, nothing begun yet).
	CHECK(u_rendering_modes_key_target_allowed(quad.data(), 2, 1, 0));
	// A stereo / legacy session (2) cannot fill the 4-view kind...
	CHECK_FALSE(u_rendering_modes_key_target_allowed(quad.data(), 2, 1, 2));
	// ...a multiview session (4) can, and every session can fill 2D and stereo.
	CHECK(u_rendering_modes_key_target_allowed(quad.data(), 2, 1, 4));
	CHECK(u_rendering_modes_key_target_allowed(quad.data(), 2, 0, 2));
	CHECK(u_rendering_modes_key_target_allowed(stereo.data(), 2, 1, 2));
	// Degenerate: an out-of-range target is not allowed; no table, no opinion.
	CHECK_FALSE(u_rendering_modes_key_target_allowed(quad.data(), 2, 2, 2));
	CHECK(u_rendering_modes_key_target_allowed(nullptr, 0, 1, 2));
}

/*
 * ADR-065: "V toggles, 0 and 1 select, on every platform and every driver". On
 * a driver that does not implement OUTPUT_MODE the digits used to do nothing:
 * they only set the property, while V wrote the active index and asked for the
 * hardware switch itself. u_rendering_modes_decide_key is now the one decision
 * both key kinds go through, so a digit must decide exactly what V decides for
 * the same target.
 */
TEST_CASE("keys: V and the digit for the same target decide the same thing", "[u_rendering_modes][keys]")
{
	const std::vector<xrt_rendering_mode> vendor = {mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	                                                mk(2, 0.5f, 0.5f, true, 2, 1, "3D")};
	for (uint32_t active = 0; active < 2; active++) {
		const u_rendering_mode_key_decision v =
		    u_rendering_modes_decide_key(vendor.data(), 2, active, true, -1, false, 0);
		REQUIRE(v.action == U_RENDERING_MODE_KEY_APPLY);
		CHECK(v.target == (active == 0 ? 1u : 0u));
		const u_rendering_mode_key_decision digit =
		    u_rendering_modes_decide_key(vendor.data(), 2, active, false, (int32_t)v.target, false, 0);
		CHECK(digit.action == v.action);
		CHECK(digit.target == v.target);
		CHECK(digit.want_3d == v.want_3d);
	}
}

TEST_CASE("keys: '0' goes to 2D and '1' to 3D, hardware request included", "[u_rendering_modes][keys]")
{
	const std::vector<xrt_rendering_mode> vendor = {mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	                                                mk(2, 0.5f, 0.5f, true, 2, 1, "3D")};
	u_rendering_mode_key_decision d = u_rendering_modes_decide_key(vendor.data(), 2, 1, false, 0, false, 0);
	CHECK(d.action == U_RENDERING_MODE_KEY_APPLY);
	CHECK(d.target == 0);
	CHECK_FALSE(d.want_3d);
	d = u_rendering_modes_decide_key(vendor.data(), 2, 0, false, 1, false, 0);
	CHECK(d.action == U_RENDERING_MODE_KEY_APPLY);
	CHECK(d.target == 1);
	CHECK(d.want_3d);
}

TEST_CASE("keys: the digit for the mode already in effect does nothing", "[u_rendering_modes][keys]")
{
	// No index write, no hardware request, and on the service no queued
	// transition to where the device already is (`active` there is the queued
	// target when one is pending).
	const std::vector<xrt_rendering_mode> vendor = {mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	                                                mk(2, 0.5f, 0.5f, true, 2, 1, "3D")};
	CHECK(u_rendering_modes_decide_key(vendor.data(), 2, 1, false, 1, false, 0).action ==
	      U_RENDERING_MODE_KEY_NOTHING);
	CHECK(u_rendering_modes_decide_key(vendor.data(), 2, 0, false, 0, false, 0).action ==
	      U_RENDERING_MODE_KEY_NOTHING);
	// ...even when the fill check would refuse it: nothing to refuse.
	const std::vector<xrt_rendering_mode> quad = {mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	                                              mk(4, 0.5f, 0.5f, true, 2, 2, "Quad")};
	CHECK(u_rendering_modes_decide_key(quad.data(), 2, 1, false, 1, false, 2).action ==
	      U_RENDERING_MODE_KEY_NOTHING);
	// V is unaffected: it always moves to the other mode.
	CHECK(u_rendering_modes_decide_key(vendor.data(), 2, 1, true, -1, false, 0).action ==
	      U_RENDERING_MODE_KEY_APPLY);
}

TEST_CASE("keys: the #1499 fill check refuses V and '1' alike", "[u_rendering_modes][keys]")
{
	const std::vector<xrt_rendering_mode> quad = {mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	                                              mk(4, 0.5f, 0.5f, true, 2, 2, "Quad")};
	const u_rendering_mode_key_decision v = u_rendering_modes_decide_key(quad.data(), 2, 0, true, -1, false, 2);
	const u_rendering_mode_key_decision one = u_rendering_modes_decide_key(quad.data(), 2, 0, false, 1, false, 2);
	CHECK(v.action == U_RENDERING_MODE_KEY_REFUSED);
	CHECK(one.action == U_RENDERING_MODE_KEY_REFUSED);
	CHECK(v.target == 1);
	CHECK(one.target == 1);
	// '0' is always fillable; a multiview session fills the quad.
	CHECK(u_rendering_modes_decide_key(quad.data(), 2, 1, false, 0, false, 2).action == U_RENDERING_MODE_KEY_APPLY);
	CHECK(u_rendering_modes_decide_key(quad.data(), 2, 0, false, 1, false, 4).action == U_RENDERING_MODE_KEY_APPLY);
}

TEST_CASE("keys: no table means V is hardware-only and a digit does nothing", "[u_rendering_modes][keys]")
{
	// No head / one mode: V falls back to qwerty's own flag...
	u_rendering_mode_key_decision d = u_rendering_modes_decide_key(nullptr, 0, 0, true, -1, true, 0);
	CHECK(d.action == U_RENDERING_MODE_KEY_HARDWARE_ONLY);
	CHECK_FALSE(d.want_3d);
	d = u_rendering_modes_decide_key(nullptr, 0, 0, true, -1, false, 0);
	CHECK(d.action == U_RENDERING_MODE_KEY_HARDWARE_ONLY);
	CHECK(d.want_3d);
	const std::vector<xrt_rendering_mode> one = {mk(1, 1.0f, 1.0f, false, 1, 1, "2D")};
	d = u_rendering_modes_decide_key(one.data(), 1, 0, true, -1, false, 0);
	CHECK(d.action == U_RENDERING_MODE_KEY_HARDWARE_ONLY);
	// ...but a digit names a mode that does not exist: '1' must not ask the
	// display processor for 3D on a device with no 3D mode, '0' does nothing.
	CHECK(u_rendering_modes_decide_key(one.data(), 1, 0, false, 1, false, 0).action ==
	      U_RENDERING_MODE_KEY_NOTHING);
	CHECK(u_rendering_modes_decide_key(one.data(), 1, 0, false, 0, false, 0).action ==
	      U_RENDERING_MODE_KEY_NOTHING);
	CHECK(u_rendering_modes_decide_key(nullptr, 0, 0, false, 1, false, 0).action ==
	      U_RENDERING_MODE_KEY_NOTHING);
	CHECK(u_rendering_modes_decide_key(nullptr, 0, 0, false, 0, false, 0).action ==
	      U_RENDERING_MODE_KEY_NOTHING);

	const std::vector<xrt_rendering_mode> vendor = {mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	                                                mk(2, 0.5f, 0.5f, true, 2, 1, "3D")};
	CHECK(u_rendering_modes_decide_key(vendor.data(), 2, 0, false, -1, false, 0).action ==
	      U_RENDERING_MODE_KEY_NOTHING);
	CHECK(u_rendering_modes_decide_key(vendor.data(), 2, 0, false, 2, false, 0).action ==
	      U_RENDERING_MODE_KEY_NOTHING);
	CHECK(u_rendering_modes_decide_key(nullptr, 0, 0, false, 5, false, 0).action == U_RENDERING_MODE_KEY_NOTHING);
}


/*
 *
 * ADR-066 C1: 3D by position, live remaps, the per-screen choice, the
 * start-up-only listing and the status the runtime computes.
 *
 */

namespace {

xrt_rendering_mode
passthrough_entry()
{
	xrt_rendering_mode m = mk(1, 1.0f, 1.0f, false, 1, 1, "Same as 2D");
	m.mode_flags |= XRT_RENDERING_MODE_FLAG_3D_PASSTHROUGH;
	return m;
}

//! A driver with a live config: CONFIGURED_3D_MODE answers `configured` (its numbering).
struct config_driver
{
	xrt_device xdev{};
	xrt_hmd_parts hmd{};
	int32_t configured = 1;
	bool answers = true;
	std::vector<int32_t> set_calls;
	std::vector<uint32_t> seen_views; //!< rendering_modes[value].view_count during OUTPUT_MODE
	bool destroyed = false;

	static config_driver *
	of(xrt_device *x)
	{
		return reinterpret_cast<config_driver *>(x);
	}

	static xrt_result_t
	set_prop(xrt_device *x, xrt_device_property_id p, int32_t v)
	{
		config_driver *d = of(x);
		if (p != XRT_DEVICE_PROPERTY_OUTPUT_MODE || v < 0 || (uint32_t)v >= x->rendering_mode_count) {
			return XRT_ERROR_NOT_IMPLEMENTED;
		}
		d->set_calls.push_back(v);
		d->seen_views.push_back(x->rendering_modes[v].view_count);
		x->hmd->active_rendering_mode_index = (uint32_t)v;
		return XRT_SUCCESS;
	}

	static xrt_result_t
	get_prop(xrt_device *x, xrt_device_property_id p, int32_t *out)
	{
		config_driver *d = of(x);
		if (p == XRT_DEVICE_PROPERTY_CONFIGURED_3D_MODE && d->answers) {
			*out = d->configured;
			return XRT_SUCCESS;
		}
		return XRT_ERROR_NOT_IMPLEMENTED;
	}

	static void
	destroy(xrt_device *x)
	{
		of(x)->destroyed = true;
	}

	explicit config_driver(const std::vector<xrt_rendering_mode> &v, int32_t config = 1)
	{
		configured = config;
		xdev.rendering_mode_count = (uint32_t)v.size();
		for (uint32_t i = 0; i < v.size(); i++) {
			xdev.rendering_modes[i] = v[i];
			xdev.rendering_modes[i].mode_index = i;
		}
		hmd.view_count = 4;
		xdev.hmd = &hmd;
		xdev.set_property = set_prop;
		xdev.get_property = get_prop;
		xdev.destroy = destroy;
		std::snprintf(xdev.str, sizeof(xdev.str), "config driver");
	}

	~config_driver()
	{
		if (!destroyed && xdev.destroy != nullptr) {
			xdev.destroy(&xdev);
		}
	}
};

//! sim_display's list shape: 2D, Same as 2D, Anaglyph, Cropped SBS, Quad (start-up-only unless declared).
std::vector<xrt_rendering_mode>
sim_like_list(bool quad_declared)
{
	std::vector<xrt_rendering_mode> v = {
	    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),      passthrough_entry(),
	    mk(2, 0.5f, 0.5f, true, 2, 1, "Anaglyph"), mk(2, 0.5f, 0.5f, true, 1, 2, "Cropped SBS"),
	    mk(4, 0.5f, 0.5f, true, 2, 2, "Quad"),
	};
	if (!quad_declared) {
		v[4].mode_flags |= XRT_RENDERING_MODE_FLAG_START_UP_ONLY;
	}
	return v;
}

char g_choice[32] = {0};
uint32_t g_choice_source = 0;

bool
test_resolver(xrt_device *, char *out, size_t cap, uint32_t *src)
{
	if (g_choice[0] == '\0') {
		return false;
	}
	std::snprintf(out, cap, "%s", g_choice);
	*src = g_choice_source;
	return true;
}

uint32_t
proxy_hook(xrt_device *)
{
	return 9;
}

} // namespace

TEST_CASE("C1: slot 1 is 3D by position, a passthrough entry included", "[u_rendering_modes][adr066]")
{
	const std::vector<xrt_rendering_mode> slots = {mk(1, 1.0f, 1.0f, false, 1, 1, "2D"), passthrough_entry()};
	CHECK(u_rendering_mode_is_3d(&slots[1]));
	CHECK_FALSE(u_rendering_mode_is_flat(&slots[1]));
	CHECK(u_rendering_modes_index_is_3d(slots.data(), 2, 1));
	CHECK_FALSE(u_rendering_modes_index_is_3d(slots.data(), 2, 0));
	// The passthrough slot does not ask for the lens.
	CHECK_FALSE(u_rendering_modes_index_wants_hw_3d(slots.data(), 2, 1));
	// V goes 1 -> 0 -> 1 (it used to stay on a flat one-view slot 1).
	CHECK(u_rendering_modes_toggle_target(slots.data(), 2, 1) == 0);
	CHECK(u_rendering_modes_toggle_target(slots.data(), 2, 0) == 1);
	const u_rendering_mode_key_decision d = u_rendering_modes_decide_key(slots.data(), 2, 0, true, -1, false, 2);
	CHECK(d.action == U_RENDERING_MODE_KEY_APPLY);
	CHECK(d.target == 1);
	CHECK_FALSE(d.want_3d); // the lens stays 2D
}

TEST_CASE("C1: choice names are the slug of the driver's mode name", "[u_rendering_modes][adr066]")
{
	char b[32];
	u_rendering_mode_slug("Same as 2D", b, sizeof(b));
	CHECK(std::string(b) == "same_as_2d");
	u_rendering_mode_slug("Cropped SBS", b, sizeof(b));
	CHECK(std::string(b) == "cropped_sbs");
	u_rendering_mode_slug("  Quad!! ", b, sizeof(b));
	CHECK(std::string(b) == "quad");
	const xrt_rendering_mode m = mk(2, 0.5f, 0.5f, true, 2, 1, "Squeezed SBS");
	CHECK(u_rendering_mode_matches_choice(&m, "squeezed_sbs"));
	CHECK(u_rendering_mode_matches_choice(&m, "Squeezed SBS"));
	CHECK_FALSE(u_rendering_mode_matches_choice(&m, "sbs"));
}

TEST_CASE("C1: a remap swaps the slot-1 entry in place and keeps the full list", "[u_rendering_modes][adr066]")
{
	table t(sim_like_list(true), 0);
	u_rendering_modes_trim_info info{};
	REQUIRE(u_rendering_modes_trim(t.modes, &t.count, &t.active, 2, &info) == U_RENDERING_MODES_TRIM_TRIMMED);
	REQUIRE(std::string(t.modes[1].mode_name) == "Anaglyph");
	REQUIRE(info.envelope_count == 5);
	CHECK(t.active == 0);

	// Anaglyph -> Quad -> Same as 2D -> Anaglyph: always the same 5 entries.
	CHECK(u_rendering_modes_remap(t.modes, 5, 2, 4, &info));
	CHECK(std::string(t.modes[1].mode_name) == "Quad");
	CHECK(info.to_driver[1] == 4);
	CHECK(u_rendering_modes_remap(t.modes, 5, 4, 1, &info));
	CHECK(std::string(t.modes[1].mode_name) == "Same as 2D");
	CHECK(u_rendering_modes_remap(t.modes, 5, 1, 2, &info));
	CHECK(std::string(t.modes[1].mode_name) == "Anaglyph");
	CHECK(std::string(t.modes[2].mode_name) == "Same as 2D");
	for (uint32_t i = 0; i < 5; i++) {
		CHECK(t.modes[i].mode_index == i);
	}
	// Refused: 0 (the 2D entry), out of range, the current one.
	CHECK_FALSE(u_rendering_modes_remap(t.modes, 5, 2, 0, &info));
	CHECK_FALSE(u_rendering_modes_remap(t.modes, 5, 2, 5, &info));
	CHECK_FALSE(u_rendering_modes_remap(t.modes, 5, 2, 2, &info));
}

TEST_CASE("C1: a start-up-only listing stays out of the envelope and is never selected", "[u_rendering_modes][adr066]")
{
	table t(sim_like_list(false), 0);
	u_rendering_modes_trim_info info{};
	REQUIRE(u_rendering_modes_trim(t.modes, &t.count, &t.active, 4, &info) == U_RENDERING_MODES_TRIM_TRIMMED);
	CHECK(info.original_count == 5);
	CHECK(info.envelope_count == 4);
	CHECK(std::string(t.modes[1].mode_name) == "Same as 2D"); // Quad refused: the first selectable
	CHECK(u_rendering_modes_max_view_count(t.modes, info.envelope_count) == 2);
	CHECK_FALSE(u_rendering_modes_remap(t.modes, info.envelope_count, 1, 4, &info));
}

TEST_CASE("C1: an entry wider than XRT_MAX_VIEWS is never selected or sized for", "[u_rendering_modes][adr066]")
{
	std::vector<xrt_rendering_mode> v = {mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	                                     mk(12, 0.25f, 0.25f, true, 4, 3, "Light field"),
	                                     mk(2, 0.5f, 0.5f, true, 2, 1, "Stereo")};
	table t(v, 1);
	u_rendering_modes_trim_info info{};
	REQUIRE(u_rendering_modes_trim(t.modes, &t.count, &t.active, 1, &info) == U_RENDERING_MODES_TRIM_TRIMMED);
	CHECK(std::string(t.modes[1].mode_name) == "Stereo");
	CHECK(u_rendering_modes_max_view_count(t.modes, info.envelope_count) == 2);
}

TEST_CASE("C1: the driver's config applies live: remap, generation, OUTPUT_MODE re-issued",
          "[u_rendering_modes][adr066][device]")
{
	u_rendering_modes_set_choice_resolver(nullptr);
	config_driver d(sim_like_list(true), /* Same as 2D */ 1);
	REQUIRE(u_device_enforce_two_rendering_modes(&d.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
	REQUIRE(std::string(d.xdev.rendering_modes[1].mode_name) == "Same as 2D");
	REQUIRE(d.hmd.view_count == 4); // the envelope's max, Quad declared
	const uint32_t gen0 = u_device_rendering_modes_generation(&d.xdev);

	SECTION("in 2D: the table moves, nothing reaches the driver")
	{
		d.configured = 2;
		CHECK(u_device_rendering_modes_refresh_config(&d.xdev, "sim-display"));
		CHECK(u_device_rendering_modes_generation(&d.xdev) == gen0 + 1);
		CHECK(std::string(d.xdev.rendering_modes[1].mode_name) == "Anaglyph");
		CHECK(d.xdev.rendering_mode_count == 2);
		CHECK(d.hmd.active_rendering_mode_index == 0);
		CHECK(d.set_calls.empty());
		CHECK(d.hmd.view_count == 4); // never re-sized
		// No change: no remap, no bump.
		CHECK_FALSE(u_device_rendering_modes_refresh_config(&d.xdev, "sim-display"));
		CHECK(u_device_rendering_modes_generation(&d.xdev) == gen0 + 1);
	}
	SECTION("in 3D: the index stays 1 and the driver is told its new entry")
	{
		CHECK(xrt_device_set_property(&d.xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, 1) == XRT_SUCCESS);
		CHECK(d.set_calls.back() == 1);
		d.configured = 4; // Quad
		CHECK(u_device_rendering_modes_refresh_config(&d.xdev, "sim-display"));
		CHECK(d.hmd.active_rendering_mode_index == 1);
		CHECK(d.set_calls.back() == 4);  // its own numbering
		CHECK(d.seen_views.back() == 4); // and it read ITS Quad entry
		CHECK(std::string(d.xdev.rendering_modes[1].mode_name) == "Quad");
		CHECK(d.xdev.rendering_mode_count == 2);
	}
	SECTION("a value outside the declared 3D entries is refused, the slot kept")
	{
		d.configured = 0;
		CHECK_FALSE(u_device_rendering_modes_refresh_config(&d.xdev, "sim-display"));
		d.configured = 7;
		CHECK_FALSE(u_device_rendering_modes_refresh_config(&d.xdev, "sim-display"));
		CHECK(u_device_rendering_modes_generation(&d.xdev) == gen0);
		CHECK(std::string(d.xdev.rendering_modes[1].mode_name) == "Same as 2D");
	}
}

TEST_CASE("C1: the per-screen choice outranks the driver's default", "[u_rendering_modes][adr066][device]")
{
	config_driver d(sim_like_list(false), 1);
	u_rendering_modes_set_choice_resolver(test_resolver);
	g_choice[0] = '\0';
	REQUIRE(u_device_enforce_two_rendering_modes(&d.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
	REQUIRE(d.hmd.view_count == 2); // Quad listed start-up-only: not in the envelope

	std::snprintf(g_choice, sizeof(g_choice), "%s", "cropped_sbs");
	g_choice_source = XRT_STATUS_MODE_3D_SOURCE_USER;
	CHECK(u_device_rendering_modes_refresh_config(&d.xdev, "sim-display"));
	CHECK(std::string(d.xdev.rendering_modes[1].mode_name) == "Cropped SBS");

	xrt_status_mode_3d st{};
	u_device_rendering_modes_fill_status(&d.xdev, &st);
	CHECK(st.valid);
	CHECK(std::string(st.active) == "cropped_sbs");
	CHECK(std::string(st.setting) == "cropped_sbs");
	CHECK(st.source == XRT_STATUS_MODE_3D_SOURCE_USER);
	// The driver's order, every 3D listing, with what the runtime says about each.
	REQUIRE(st.choice_count == 4);
	CHECK(std::string(st.choices[0].name) == "same_as_2d");
	CHECK(st.choices[0].passthrough);
	CHECK(st.choices[0].views == 1);
	CHECK(st.choices[0].applies == XRT_STATUS_MODE_3D_APPLIES_NOW);
	CHECK(std::string(st.choices[1].label) == "Anaglyph");
	CHECK(std::string(st.choices[3].name) == "quad");
	CHECK(st.choices[3].applies == XRT_STATUS_MODE_3D_APPLIES_RESTART_APP);
	CHECK(std::string(st.choices[3].reason).find("start-up") != std::string::npos);

	SECTION("a choice the list cannot serve falls back to the driver's default")
	{
		std::snprintf(g_choice, sizeof(g_choice), "%s", "quad");
		d.configured = 2;
		CHECK(u_device_rendering_modes_refresh_config(&d.xdev, "sim-display"));
		CHECK(std::string(d.xdev.rendering_modes[1].mode_name) == "Anaglyph");
		u_device_rendering_modes_fill_status(&d.xdev, &st);
		CHECK(st.source == XRT_STATUS_MODE_3D_SOURCE_DRIVER_DEFAULT);
	}
	SECTION("cleared: the driver's default again")
	{
		g_choice[0] = '\0';
		CHECK(u_device_rendering_modes_refresh_config(&d.xdev, "sim-display"));
		CHECK(std::string(d.xdev.rendering_modes[1].mode_name) == "Same as 2D");
	}
	u_rendering_modes_set_choice_resolver(nullptr);
	g_choice[0] = '\0';
}

TEST_CASE("C1: the in-process sync polls at most once a second; an IPC proxy uses its hook",
          "[u_rendering_modes][adr066][device]")
{
	u_rendering_modes_set_choice_resolver(nullptr);
	config_driver d(sim_like_list(true), 1);
	REQUIRE(u_device_enforce_two_rendering_modes(&d.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
	const uint64_t s = 1000ull * 1000ull * 1000ull;
	const uint32_t g0 = u_device_rendering_modes_sync(&d.xdev, 10 * s); // first poll: immediate
	d.configured = 2;
	CHECK(u_device_rendering_modes_sync(&d.xdev, 10 * s + s / 2) == g0); // throttled
	CHECK(u_device_rendering_modes_sync(&d.xdev, 11 * s) == g0 + 1);     // applied

	// An IPC head proxy: published envelope, generation from its hook.
	xrt_device proxy{};
	proxy.rendering_mode_count = 2;
	u_device_rendering_modes_publish(&proxy, 5, 3);
	CHECK(u_device_rendering_modes_envelope_count(&proxy) == 5);
	CHECK(u_device_rendering_modes_generation(&proxy) == 3);
	u_device_rendering_modes_set_sync_hook(&proxy, proxy_hook);
	CHECK(u_device_rendering_modes_sync(&proxy, 0) == 9);
	u_device_rendering_modes_forget(&proxy);
	CHECK(u_device_rendering_modes_envelope_count(&proxy) == 2);
}
