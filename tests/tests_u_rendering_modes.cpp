// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-065: the two-mode contract — a display driver's run-time table is
 *         [0] = 2D, [1] = 3D, and the runtime trims a longer one.
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

#include <cstdio>
#include <cstring>
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
		CHECK(u_rendering_modes_trim(modes, &count, &active, &info) == expect);
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
	CHECK(info.dropped_mask == 0);
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
		CHECK(info.dropped_mask == (1u << 2));
		CHECK(std::strcmp(t.modes[1].mode_name, "3D") == 0);
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
		CHECK(info.dropped_mask == (1u << 1));
		CHECK(std::strcmp(t.modes[1].mode_name, "3D (variant)") == 0);
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
		// Every original mode but 0 and the kept one is dropped.
		for (uint32_t i = 0; i < 6; i++) {
			const bool dropped = (info.dropped_mask & (1u << i)) != 0;
			CHECK(dropped == (i != 0 && i != r.expect_kept));
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
	CHECK(info.dropped_mask == 0);
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

TEST_CASE("the max view count and the worst-case atlas come from the trimmed table", "[u_rendering_modes]")
{
	// A driver whose dropped mode was the widest AND the largest: a 3x3 quilt
	// at half scale is 1.5x the panel. Untrimmed it would size every swapchain
	// for 9 views and 2880x1620; trimmed to {2D, stereo} it is 2 views and the
	// panel. (The case the sizing note in docs/specs/runtime/multiview-tiling.md
	// is about: the 2D mode dominates the shipping tables, not every table.)
	const std::vector<xrt_rendering_mode> v = {
	    mk(1, 1.0f, 1.0f, false, 1, 1, "2D"),
	    mk(2, 0.5f, 1.0f, true, 2, 1, "Stereo"),
	    mk(9, 0.5f, 0.5f, true, 3, 3, "Quilt 3x3"),
	};
	const uint32_t W = 1920, H = 1080;

	table untrimmed(v, 1);
	for (uint32_t i = 0; i < untrimmed.count; i++) {
		u_tiling_compute_mode(&untrimmed.modes[i], W, H);
	}
	uint32_t uw = 0, uh = 0;
	u_tiling_compute_system_atlas(untrimmed.modes, untrimmed.count, &uw, &uh);
	CHECK(uw == 2880);
	CHECK(uh == 1620);
	CHECK(u_rendering_modes_max_view_count(untrimmed.modes, untrimmed.count) == 9);

	SECTION("started in the stereo mode: the quilt is dropped")
	{
		table t(v, 1);
		t.trim(U_RENDERING_MODES_TRIM_TRIMMED);
		for (uint32_t i = 0; i < t.count; i++) {
			u_tiling_compute_mode(&t.modes[i], W, H);
		}
		uint32_t aw = 0, ah = 0;
		u_tiling_compute_system_atlas(t.modes, t.count, &aw, &ah);
		CHECK(aw == W);
		CHECK(ah == H);
		CHECK(u_rendering_modes_max_view_count(t.modes, t.count) == 2);
	}
	SECTION("started in the quilt: the quilt is the 3D mode and still sets the size")
	{
		table t(v, 2);
		t.trim(U_RENDERING_MODES_TRIM_TRIMMED);
		for (uint32_t i = 0; i < t.count; i++) {
			u_tiling_compute_mode(&t.modes[i], W, H);
		}
		uint32_t aw = 0, ah = 0;
		u_tiling_compute_system_atlas(t.modes, t.count, &aw, &ah);
		CHECK(aw == 2880);
		CHECK(ah == 1620);
		CHECK(u_rendering_modes_max_view_count(t.modes, t.count) == 9);
	}
}

TEST_CASE("degenerate trim inputs are harmless", "[u_rendering_modes]")
{
	uint32_t count = 3, active = 0;
	CHECK(u_rendering_modes_trim(nullptr, &count, &active, nullptr) == U_RENDERING_MODES_TRIM_UNCHANGED);
	CHECK(count == 3);
	xrt_rendering_mode m[XRT_MAX_RENDERING_MODES]{};
	CHECK(u_rendering_modes_trim(m, nullptr, &active, nullptr) == U_RENDERING_MODES_TRIM_UNCHANGED);
	// A NULL active is allowed: the trim picks the first 3D mode.
	table t(old_sim_table(), 0);
	CHECK(u_rendering_modes_trim(t.modes, &t.count, nullptr, nullptr) == U_RENDERING_MODES_TRIM_TRIMMED);
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
	CHECK(h.xdev.set_property == fake_head::set_prop); // not wrapped
	CHECK(h.hmd.active_rendering_mode_index == 1);
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

TEST_CASE("enforce: started in 2D, the device max view count drops to the kept mode's", "[u_rendering_modes][device]")
{
	fake_head h(old_sim_table(), 0);
	CHECK(u_device_enforce_two_rendering_modes(&h.xdev, "sim-display") == U_RENDERING_MODES_TRIM_TRIMMED);
	CHECK(h.hmd.active_rendering_mode_index == 0);
	CHECK(h.hmd.view_count == 2); // was 4 (Quad), Quad dropped
	CHECK(h.xdev.set_property == fake_head::set_prop);
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
